module conformance.p44c_qualified_shape_aggregate

record Pair { left: Int64, right: Bool }
enum Failure { failed }

function checked() -> Result<Pair, Failure> effects { pure }
ensures { success => true }
{
    return ok(Pair { left = 42, right = true })
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked()
    return if value == ok(Pair { left = 42, right = true }) { 42 } else { 0 }
}
