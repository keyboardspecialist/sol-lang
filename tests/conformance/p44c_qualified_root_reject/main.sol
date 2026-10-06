module conformance.p44c_qualified_root_reject

enum Failure { failed }

function checked() -> Result<Int64, Failure> effects { pure }
ensures { success => result == 42 }
{
    return ok(42)
}
