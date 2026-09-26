module expense_policy.visible_tests

use expense_policy.domain.ExpenseClaim
use expense_policy.policy.decide

function matches(
    amount_cents: Int64,
    category: Text,
    receipt: Bool,
    employee_active: Bool,
    domestic: Bool,
    submitted_days_ago: Int64,
    outcome: Text,
    reason: Text,
    reimbursable_cents: Int64,
) -> Bool
effects { pure }
{
    let result = decide(ExpenseClaim {
            amount_cents = amount_cents,
            category = category,
            receipt = receipt,
            employee_active = employee_active,
            domestic = domestic,
            submitted_days_ago = submitted_days_ago,
        })
    return result.outcome == outcome
    && result.reason == reason
    && result.reimbursable_cents == reimbursable_cents
}

test "unknown category has first precedence" {
    matches(-1, "other", false, false, false, -1, "reject", "unknown_category", 0)
}

test "inactive employee precedes invalid values" {
    matches(0, "meal", false, false, true, -1, "reject", "inactive_employee", 0)
}

test "zero amount is invalid" {
    matches(0, "meal", true, true, true, 0, "reject", "invalid_claim", 0)
}

test "negative submission age is invalid" {
    matches(1000, "meal", true, true, true, -1, "reject", "invalid_claim", 0)
}

test "claim older than ninety days is stale" {
    matches(1000, "supplies", true, true, true, 91, "reject", "stale_claim", 0)
}

test "international travel requires review first" {
    matches(120000, "travel", false, true, false, 10, "manual_review", "international_travel", 120000)
}

test "missing receipt above threshold requires review" {
    matches(2501, "meal", false, true, true, 1, "manual_review", "missing_receipt", 2501)
}

test "meal over cap is bounded" {
    matches(7501, "meal", true, true, true, 1, "manual_review", "category_cap_exceeded", 7500)
}

test "meal at cap is approved" {
    matches(7500, "meal", true, true, true, 90, "approve", "eligible", 7500)
}

test "travel at cap is approved" {
    matches(100000, "travel", true, true, true, 90, "approve", "eligible", 100000)
}

test "supplies at cap is approved" {
    matches(25000, "supplies", true, true, true, 90, "approve", "eligible", 25000)
}

test "receipt threshold is inclusive" {
    matches(2500, "meal", false, true, true, 90, "approve", "eligible", 2500)
}
