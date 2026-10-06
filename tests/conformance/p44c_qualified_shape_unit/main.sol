module conformance.p44c_qualified_shape_unit

enum Failure { failed }

function checked() -> Result<(), Failure> effects { pure }
ensures { success => true }
{
    return ok(())
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked()
    return if value == ok(()) { 42 } else { 0 }
}
