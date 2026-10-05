module conformance.p44c_ensures_callee_failure

function fail() -> Int64 effects { pure } {
    unreachable because { true }
}

@entry
public function launch() -> Int64 effects { pure } ensures { false } {
    return fail()
}
