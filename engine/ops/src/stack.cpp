// SPDX-License-Identifier: Apache-2.0
#include "f3d/ops/stack.hpp"

#include "f3d/core/assert.hpp"
#include "f3d/core/profile.hpp"

#include <cstddef>
#include <utility>

namespace f3d::ops {

// --- ChangeBus -------------------------------------------------------------------------------

ChangeBus::Token ChangeBus::subscribe(Callback callback) {
    const Token token = next_++;
    subscribers_.emplace(token, std::move(callback));
    return token;
}

void ChangeBus::unsubscribe(Token token) {
    subscribers_.erase(token);
}

void ChangeBus::emit(const ChangeEvent& event) const {
    for (const auto& [token, callback] : subscribers_) {
        (void)token;
        if (callback) {
            callback(event);
        }
    }
}

// --- Transaction -----------------------------------------------------------------------------

Transaction::Transaction(std::string name) : name_(std::move(name)) {}

void Transaction::add(std::unique_ptr<Operation> op, bool merge) {
    if (merge) {
        for (auto it = ops_.rbegin(); it != ops_.rend(); ++it) {
            if ((*it)->merge(*op)) {
                return;
            }
        }
    }
    ops_.push_back(std::move(op));
}

bool Transaction::execute(Context& context) {
    for (const std::unique_ptr<Operation>& op : ops_) {
        if (!op->execute(context)) {
            return false;
        }
    }
    return true;
}

void Transaction::undo(Context& context) {
    for (auto it = ops_.rbegin(); it != ops_.rend(); ++it) {
        (*it)->undo(context);
    }
}

std::size_t Transaction::memory_size() const {
    std::size_t total = sizeof(*this);
    for (const std::unique_ptr<Operation>& op : ops_) {
        total += op->memory_size();
    }
    return total;
}

// --- OpStack ---------------------------------------------------------------------------------

OpStack::OpStack(Context context) : context_(context) {}

bool OpStack::execute(std::unique_ptr<Operation> op, bool merge) {
    if (!op) {
        return false;
    }
    F3D_ZONE;
    if (!op->execute(context_)) {
        return false;
    }
    if (!op->records_history()) {
        return true;
    }
    if (!open_.empty()) {
        open_.back()->add(std::move(op), merge);
        return true;
    }
    redo_.clear();
    if (merge && !undo_.empty() && undo_.back().op->merge(*op)) {
        Entry& last = undo_.back();
        undo_bytes_ -= last.bytes;
        last.bytes = last.op->memory_size();
        last.id = next_id_++; // a different state than before the merge
        undo_bytes_ += last.bytes;
        trim();
        return true;
    }
    push(std::move(op));
    return true;
}

void OpStack::begin_transaction(std::string name) {
    open_.push_back(std::make_unique<Transaction>(std::move(name)));
}

bool OpStack::end_transaction() {
    if (open_.empty()) {
        return false;
    }
    std::unique_ptr<Transaction> transaction = std::move(open_.back());
    open_.pop_back();
    if (transaction->size() == 0) {
        return true;
    }
    if (!open_.empty()) {
        open_.back()->add(std::move(transaction));
        return true;
    }
    redo_.clear();
    push(std::move(transaction));
    return true;
}

void OpStack::abort_transaction() {
    if (open_.empty()) {
        return;
    }
    std::unique_ptr<Transaction> transaction = std::move(open_.back());
    open_.pop_back();
    transaction->undo(context_);
}

bool OpStack::undo() {
    if (undo_.empty()) {
        return false;
    }
    F3D_ZONE;
    Entry entry = std::move(undo_.back());
    undo_.pop_back();
    undo_bytes_ -= entry.bytes;
    entry.op->undo(context_);
    redo_.push_back(std::move(entry));
    return true;
}

bool OpStack::redo() {
    if (redo_.empty()) {
        return false;
    }
    F3D_ZONE;
    Entry entry = std::move(redo_.back());
    redo_.pop_back();
    if (!entry.op->execute(context_)) {
        redo_.push_back(std::move(entry)); // put it back; nothing changed
        return false;
    }
    undo_bytes_ += entry.bytes;
    undo_.push_back(std::move(entry));
    trim();
    return true;
}

std::vector<std::string> OpStack::undo_names() const {
    std::vector<std::string> names;
    names.reserve(undo_.size());
    for (const Entry& entry : undo_) {
        names.push_back(entry.op->display_name());
    }
    return names;
}

std::vector<std::string> OpStack::redo_names() const {
    std::vector<std::string> names;
    names.reserve(redo_.size());
    for (auto it = redo_.rbegin(); it != redo_.rend(); ++it) {
        names.push_back(it->op->display_name());
    }
    return names;
}

void OpStack::set_limits(std::size_t max_entries, std::size_t max_bytes) {
    max_entries_ = max_entries == 0 ? 1 : max_entries;
    max_bytes_ = max_bytes;
    trim();
}

void OpStack::clear() {
    undo_.clear();
    redo_.clear();
    open_.clear();
    undo_bytes_ = 0;
}

void OpStack::push(std::unique_ptr<Operation> op) {
    const std::size_t bytes = op->memory_size();
    undo_bytes_ += bytes;
    undo_.push_back({std::move(op), bytes, next_id_++});
    trim();
}

void OpStack::trim() {
    // Oldest first. The newest entry is always kept, even if it alone exceeds the byte cap.
    std::size_t evict = undo_.size() > max_entries_ ? undo_.size() - max_entries_ : 0;
    std::size_t bytes = undo_bytes_;
    for (std::size_t i = 0; i < evict; ++i) {
        bytes -= undo_[i].bytes;
    }
    while (max_bytes_ > 0 && evict + 1 < undo_.size() && bytes > max_bytes_) {
        bytes -= undo_[evict].bytes;
        ++evict;
    }
    if (evict > 0) {
        undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(evict));
        undo_bytes_ = bytes;
    }
}

} // namespace f3d::ops
