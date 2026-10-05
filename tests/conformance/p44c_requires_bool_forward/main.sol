module conformance.p44c_requires_bool_forward

function forward(predicate: Bool) -> Int64 effects { pure } requires {
    true && predicate
} {
    return 42
}

@entry
public function launch() -> Int64 effects { pure } {
    return forward(true)
}

public function fail() -> Int64 effects { pure } {
    return forward(false)
}
