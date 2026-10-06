module conformance.p44c_refined_text_reject

type AnyText = refined Text where true

function checked(value: Text) -> AnyText effects { pure } {
    return AnyText(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked("text")
    if value == value { return 42 } else { return 0 }
}
