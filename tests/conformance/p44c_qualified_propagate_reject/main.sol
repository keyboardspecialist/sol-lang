module conformance.p44c_qualified_propagate_reject

enum Failure { failed }

function checked(value: Result<Int64, Failure>) -> Result<Int64, Failure>
effects { pure }
ensures {
    success => result > 0
    failure => true
}
{
    let item = value?
    return ok(item)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(ok(42))
    return if value == ok(42) { 42 } else { 0 }
}
