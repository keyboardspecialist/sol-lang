module conformance.p44c_requires_parameter_forward

function forward(value: Int64) -> Int64 effects { pure } requires {
    value > 0 || value < 0
} {
    return value
}

@entry
public function launch() -> Int64 effects { pure } {
    return forward(42)
}

public function fail() -> Int64 effects { pure } {
    return forward(0)
}
