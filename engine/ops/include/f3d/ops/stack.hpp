// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/ops/operation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace f3d::ops {

/// A group of operations that undo/redo as one unit. Operations executed while a
/// transaction is open are appended to it.
class Transaction : public Operation {
public:
    explicit Transaction(std::string name);

    [[nodiscard]] std::string_view id() const override { return "transaction"; }
    [[nodiscard]] std::string display_name() const override { return name_; }
    [[nodiscard]] entt::meta_type params_type() const override { return {}; }
    [[nodiscard]] nlohmann::json params_json() const override { return nlohmann::json::object(); }
    void set_params_json(const nlohmann::json&) override {}

    /// Re-runs every child in order (used by redo; children are executed
    /// individually while the transaction is being built).
    bool execute(Context& context) override;
    /// Undoes every child in reverse order.
    void undo(Context& context) override;

    /// Appends `op`. With `merge`, first tries to fold it into an earlier child that
    /// accepts it (the same entity/property edited again during a drag).
    void add(std::unique_ptr<Operation> op, bool merge = false);
    [[nodiscard]] std::size_t size() const noexcept { return ops_.size(); }
    [[nodiscard]] std::size_t memory_size() const override;

private:
    std::string name_;
    std::vector<std::unique_ptr<Operation>> ops_;
};

/// Undo/redo history with transactions and a cap by entry count and bytes.
class OpStack {
public:
    explicit OpStack(Context context);

    /// Executes `op` and records it. With `merge` (a continuing edit, e.g. the
    /// second and later frames of a drag), it is folded into the previous entry
    /// (or, inside a transaction, an earlier child) when that entry accepts it.
    /// Without `merge` it always becomes a new entry, so separate edits of the
    /// same property stay separate undo steps.
    bool execute(std::unique_ptr<Operation> op, bool merge = false);

    void begin_transaction(std::string name);
    /// Commits the innermost transaction (onto its parent, or the history).
    bool end_transaction();
    /// Discards the innermost transaction, undoing what it already did.
    void abort_transaction();
    [[nodiscard]] bool transaction_open() const noexcept { return !open_.empty(); }

    bool undo();
    bool redo();

    /// Identifies the current history position: the id of the newest undo entry (0
    /// when there is none). Ids are unique and change when an entry is merged
    /// into, so "saved at state X" stays correct across undo/redo (dirty tracking).
    [[nodiscard]] std::uint64_t state_id() const noexcept { return undo_.empty() ? 0 : undo_.back().id; }

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t undo_count() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redo_count() const noexcept { return redo_.size(); }

    /// Display names of the recorded operations, oldest first (the operation log).
    [[nodiscard]] std::vector<std::string> undo_names() const;
    [[nodiscard]] std::vector<std::string> redo_names() const;

    /// Evicts the oldest entries first once either cap is exceeded (0 bytes: no byte cap).
    void set_limits(std::size_t max_entries, std::size_t max_bytes);
    [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }
    [[nodiscard]] std::size_t memory_bytes() const noexcept { return undo_bytes_; }

    void clear();

    [[nodiscard]] Context& context() noexcept { return context_; }

private:
    /// A history entry with its memory size, measured when it was recorded.
    struct Entry {
        std::unique_ptr<Operation> op;
        std::size_t bytes = 0;
        std::uint64_t id = 0;
    };

    void push(std::unique_ptr<Operation> op);
    void trim();

    Context context_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    std::size_t undo_bytes_ = 0;
    std::uint64_t next_id_ = 1;
    std::vector<std::unique_ptr<Transaction>> open_;
    std::size_t max_entries_ = 256;
    std::size_t max_bytes_ = 64u * 1024u * 1024u;
};

} // namespace f3d::ops
