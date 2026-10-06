module conformance.p44c_qualified_shape_non_enum_error

function checked(fail: Bool) -> Result<Int64, Text> effects { pure }
ensures { success => result == 42 }
{
    return if fail { err("failed") } else { ok(42) }
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(false)
    return if value == ok(42) { 42 } else { 0 }
}
