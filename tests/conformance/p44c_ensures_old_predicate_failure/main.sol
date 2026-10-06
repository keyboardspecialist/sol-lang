module conformance.p44c_ensures_old_predicate_failure

function checked(value: Int64) -> Int64 effects { pure }
ensures { result == old(value) / 0 } {
    return value
}

@entry
public function launch() -> Int64 effects { pure } {
    return checked(42)
}
