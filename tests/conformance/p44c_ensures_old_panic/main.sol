module conformance.p44c_ensures_old_panic

function fail() -> Int64 effects { panic } {
    panic "old snapshot body panic"
}

function checked(value: Int64) -> Int64 effects { panic }
ensures { result == old(value) } {
    return fail()
}

@entry
public function launch() -> Int64 effects { panic } {
    return checked(42)
}
