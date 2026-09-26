module expense_policy.domain

public enum ExpenseCategory {
    meal,
    travel,
    supplies,
    unknown,
}

public record ExpenseClaim {
    amount_cents: Int64,
    category: Text,
    receipt: Bool,
    employee_active: Bool,
    domestic: Bool,
    submitted_days_ago: Int64,
}

public record ExpenseDecision {
    outcome: Text,
    reason: Text,
    reimbursable_cents: Int64,
}

public function parse_category(value: borrow Text) -> ExpenseCategory
effects { pure }
{
    if value == "meal" {
        return ExpenseCategory.meal
    } else if value == "travel" {
        return ExpenseCategory.travel
    } else if value == "supplies" {
        return ExpenseCategory.supplies
    } else {
        return ExpenseCategory.unknown
    }
}

public function category_cap(category: ExpenseCategory) -> Int64
effects { pure }
ensures { result >= 0 }
{
    return match category {
        meal => 7500
        travel => 100000
        supplies => 25000
        unknown => 0
    }
}

public function make_decision(
    outcome: Text,
    reason: Text,
    reimbursable_cents: Int64,
) -> ExpenseDecision
effects { pure }
requires { reimbursable_cents >= 0 }
ensures {
    result.outcome == outcome
    result.reason == reason
    result.reimbursable_cents == reimbursable_cents
}
{
    return ExpenseDecision {
        outcome = outcome,
        reason = reason,
        reimbursable_cents = reimbursable_cents,
    }
}
