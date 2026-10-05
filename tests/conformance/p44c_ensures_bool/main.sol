module conformance.p44c_ensures_bool

function checked() -> Bool effects { pure } ensures { result } {
    return true
}

@entry
public function launch() -> Int64 effects { pure } {
    if checked() { return 1 } else { return 0 }
}
