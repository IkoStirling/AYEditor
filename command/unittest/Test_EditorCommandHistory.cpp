#include "AYTest.h"

#include <AYEditorCommand/EditorCommandHistory.h>

#include <memory>
#include <string>
#include <utility>

using namespace ayt::editor;

namespace {

class IntegerCommand final : public IEditorCommand {
public:
    IntegerCommand(int& value, int after, std::string key = {})
        : _value(value), _before(value), _after(after), _key(std::move(key)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override {
        if (!executeSucceeds) return false;
        _value = _after;
        return true;
    }
    bool undo() override {
        if (!undoSucceeds) return false;
        _value = _before;
        return true;
    }
    bool isAlive() const noexcept override { return alive; }
    std::string mergeKey() const override { return _key; }
    bool mergeFrom(const IEditorCommand& newer) override {
        const auto* integer = dynamic_cast<const IntegerCommand*>(&newer);
        if (integer == nullptr || integer->_key != _key) return false;
        _after = integer->_after;
        return true;
    }

    bool executeSucceeds = true;
    bool undoSucceeds = true;
    bool alive = true;

private:
    int& _value;
    int _before = 0;
    int _after = 0;
    std::string _key;
    std::string _label = "Set integer";
};

} // namespace

TEST_SUITE(EditorCommandCore)

TEST_CASE(command_history_tracks_merge_save_undo_and_redo)
{
    int value = 0;
    EditorCommandHistory history;
    int notifications = 0;
    history.setChangedCallback([&]() { ++notifications; });

    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 1, "value")));
    CHECK(value == 1);
    CHECK(history.size() == 1u);
    CHECK(history.isDirty());
    CHECK(history.markSaved());
    CHECK_FALSE(history.isDirty());

    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 2, "value")));
    CHECK(value == 2);
    CHECK(history.size() == 2u);
    CHECK(history.undo());
    CHECK(value == 1);
    CHECK_FALSE(history.isDirty());
    CHECK(history.redo());
    CHECK(value == 2);
    CHECK(history.isDirty());
    CHECK(notifications >= 5);
}

TEST_CASE(command_history_records_and_discards_already_applied_edits)
{
    int value = 4;
    EditorCommandHistory history;
    auto applied = std::make_unique<IntegerCommand>(value, 9);
    value = 9;
    CHECK(history.recordApplied(std::move(applied)));
    CHECK(history.canUndo());
    CHECK(history.isDirty());
    CHECK(history.undo());
    CHECK(value == 4);
    CHECK(history.redo());
    CHECK(value == 9);

    auto noChange = std::make_unique<IntegerCommand>(value, 9);
    CHECK(history.recordApplied(std::move(noChange)));
    CHECK(history.discardLastApplied());
    CHECK(history.size() == 1u);
    CHECK(history.cursor() == 1u);
}

TEST_CASE(command_history_commits_and_cancels_transactions_explicitly)
{
    int value = 0;
    EditorCommandHistory history;

    CHECK(history.beginTransaction("Gesture"));
    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 1)));
    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 2)));
    CHECK(history.transactionActive());
    CHECK(history.isDirty());
    CHECK_FALSE(history.canUndo());
    CHECK(history.cancelTransaction());
    CHECK(value == 0);
    CHECK(history.size() == 0u);
    CHECK_FALSE(history.isDirty());

    CHECK(history.beginTransaction("Two fields"));
    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 3)));
    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 4)));
    CHECK(history.commitTransaction());
    CHECK(history.size() == 1u);
    CHECK(history.undoLabel() == "Two fields");
    CHECK(history.undo());
    CHECK(value == 0);
    CHECK(history.redo());
    CHECK(value == 4);

    CHECK(history.beginTransaction("Empty"));
    CHECK(history.commitTransaction());
    CHECK(history.size() == 1u);
}

TEST_CASE(discard_history_never_mutates_the_document)
{
    int value = 0;
    EditorCommandHistory history;

    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 5)));
    CHECK(history.beginTransaction("Pending"));
    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 7)));
    history.discardHistory(EditorHistoryDiscardState::KeepDirty);
    CHECK(value == 7);
    CHECK(history.size() == 0u);
    CHECK_FALSE(history.transactionActive());
    CHECK(history.isDirty());

    history.discardHistory(EditorHistoryDiscardState::MarkClean);
    CHECK(value == 7);
    CHECK_FALSE(history.isDirty());
}

TEST_CASE(command_history_capacity_drops_oldest_entries_and_save_cursor)
{
    int value = 0;
    EditorCommandHistory history(2u);

    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 1)));
    CHECK(history.markSaved());
    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 2)));
    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 3)));
    CHECK(history.size() == 2u);
    CHECK(history.cursor() == 2u);
    CHECK(history.isDirty());

    CHECK(history.undo());
    CHECK(value == 2);
    CHECK(history.undo());
    CHECK(value == 1);
    CHECK_FALSE(history.canUndo());
    // The retained baseline is exactly the saved state after command 1.
    CHECK_FALSE(history.isDirty());
    CHECK_FALSE(history.setCapacity(0u));

    int unsavedValue = 0;
    EditorCommandHistory trimmedPastSave(2u);
    CHECK(trimmedPastSave.execute(
        std::make_unique<IntegerCommand>(unsavedValue, 1)));
    CHECK(trimmedPastSave.execute(
        std::make_unique<IntegerCommand>(unsavedValue, 2)));
    CHECK(trimmedPastSave.execute(
        std::make_unique<IntegerCommand>(unsavedValue, 3)));
    CHECK(trimmedPastSave.undo());
    CHECK(trimmedPastSave.undo());
    CHECK(unsavedValue == 1);
    // The original saved state (0) is outside the retained window.
    CHECK(trimmedPastSave.isDirty());
}

TEST_CASE(command_failures_preserve_cursor_and_expired_owners_fault_history)
{
    int value = 0;
    EditorCommandHistory history;

    auto initial = std::make_unique<IntegerCommand>(value, 1);
    IntegerCommand* retained = initial.get();
    CHECK(history.execute(std::move(initial)));
    retained->undoSucceeds = false;
    CHECK_FALSE(history.undo());
    CHECK(history.cursor() == 1u);
    CHECK(value == 1);
    CHECK_FALSE(history.faulted());

    retained->undoSucceeds = true;
    retained->alive = false;
    CHECK_FALSE(history.undo());
    CHECK(history.cursor() == 1u);
    CHECK(history.faulted());
    CHECK_FALSE(history.canUndo());

    history.discardHistory(EditorHistoryDiscardState::KeepDirty);
    CHECK_FALSE(history.faulted());
    CHECK(history.isDirty());
    CHECK(value == 1);
}

TEST_CASE(transaction_cancel_failure_restores_applied_transaction)
{
    int value = 0;
    EditorCommandHistory history;

    CHECK(history.beginTransaction("Atomic cancel"));
    auto first = std::make_unique<IntegerCommand>(value, 1);
    IntegerCommand* firstRetained = first.get();
    CHECK(history.execute(std::move(first)));
    CHECK(history.execute(std::make_unique<IntegerCommand>(value, 2)));

    firstRetained->undoSucceeds = false;
    CHECK_FALSE(history.cancelTransaction());
    CHECK(history.transactionActive());
    CHECK(value == 2);
    CHECK_FALSE(history.faulted());

    firstRetained->undoSucceeds = true;
    CHECK(history.cancelTransaction());
    CHECK(value == 0);
    CHECK_FALSE(history.transactionActive());
}

TEST_SUITE_END
