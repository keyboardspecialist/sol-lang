module expense_policy.policy

use expense_policy.domain.ExpenseCategory
use expense_policy.domain.ExpenseClaim
use expense_policy.domain.ExpenseDecision
use expense_policy.domain.category_cap
use expense_policy.domain.make_decision
use expense_policy.domain.parse_category

public function decide(claim: ExpenseClaim) -> ExpenseDecision
effects { pure }
ensures { result.reimbursable_cents >= 0 }
{
    let category = parse_category(claim.category)
    if category == ExpenseCategory.unknown {
        return make_decision("reject", "unknown_category", 0)
    } else if claim.employee_active == false {
        return make_decision("reject", "inactive_employee", 0)
    } else if claim.amount_cents <= 0 || claim.submitted_days_ago < 0 {
        return make_decision("reject", "invalid_claim", 0)
    } else if claim.submitted_days_ago > 90 {
        return make_decision("reject", "stale_claim", 0)
    } else if category == ExpenseCategory.travel && claim.domestic == false {
        return make_decision(
            "manual_review",
            "international_travel",
            claim.amount_cents,
        )
    } else if claim.receipt == false && claim.amount_cents > 2500 {
        return make_decision("manual_review", "missing_receipt", claim.amount_cents)
    } else {
        let cap = category_cap(category)
        if claim.amount_cents > cap {
            return make_decision("manual_review", "category_cap_exceeded", cap)
        } else {
            return make_decision("approve", "eligible", claim.amount_cents)
        }
    }
}
