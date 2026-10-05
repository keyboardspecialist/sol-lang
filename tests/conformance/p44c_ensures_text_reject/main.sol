module conformance.p44c_ensures_text_reject

function checked() -> Text effects { pure } ensures { true } {
    return "text"
}

@entry
public function launch() -> Int64 effects { pure } {
    return if checked() == "text" { 42 } else { 0 }
}
