module conformance.p44c_ensures_old_bool

function checked(value: Bool) -> Bool effects { pure }
ensures { result != old(value) } {
    return !value
}

@entry
public function launch() -> Int64 effects { pure } {
    if checked(false) { return 1 } else { return 0 }
}
