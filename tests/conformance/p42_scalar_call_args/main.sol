module conformance.p42_scalar_call_args

function helper(left: Int64, right: Bool) -> Int64 effects { pure } {
    return left
}

@entry
public function launch() -> Int64 effects { pure } {
    return helper(5, true)
}
