module conformance.p44c_qualified_shape_text

enum Failure { failed }

function checked() -> Result<Text, Failure> effects { pure }
ensures { success => true }
{
    return ok("text")
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked()
    return if value == ok("text") { 42 } else { 0 }
}
