module expense_policy.main

use expense_policy.domain.ExpenseClaim
use expense_policy.policy.decide

capability ExpenseClaimInput {
    function amount_cents() -> Int64
    effects { expense.claim.amount_cents<Self> }

    function category() -> Text
    effects { expense.claim.category<Self> }

    function receipt() -> Bool
    effects { expense.claim.receipt<Self> }

    function employee_active() -> Bool
    effects { expense.claim.employee_active<Self> }

    function domestic() -> Bool
    effects { expense.claim.domestic<Self> }

    function submitted_days_ago() -> Int64
    effects { expense.claim.submitted_days_ago<Self> }
}

capability ExpenseDecisionOutput {
    function publish(
        outcome: Text,
        reason: Text,
        reimbursable_cents: Int64,
    ) -> ()
    effects { expense.decision.publish<Self> }
}

@entry
public function evaluate(
    input: capability ExpenseClaimInput,
    output: capability ExpenseDecisionOutput,
) -> ()
effects {
    expense.claim.amount_cents<input>
    expense.claim.category<input>
    expense.claim.domestic<input>
    expense.claim.employee_active<input>
    expense.claim.receipt<input>
    expense.claim.submitted_days_ago<input>
    expense.decision.publish<output>
}
{
    let claim = ExpenseClaim {
        amount_cents = input.amount_cents(),
        category = input.category(),
        receipt = input.receipt(),
        employee_active = input.employee_active(),
        domestic = input.domestic(),
        submitted_days_ago = input.submitted_days_ago(),
    }
    let result = decide(claim)
    output.publish(result.outcome, result.reason, result.reimbursable_cents)
}
