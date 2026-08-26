module test_contract_failure

function guarded() -> Bool
requires { false }
{
    true
}

test "contract failure" guarded()
