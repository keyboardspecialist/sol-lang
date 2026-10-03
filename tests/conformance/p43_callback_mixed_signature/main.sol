module conformance.p43_callback_mixed_signature

function unary(value: Int64) -> Int64 effects { pure } { return value + 1 }
function nullary() -> Int64 effects { pure } { return 7 }

@entry
public function launch() -> Int64 effects { pure } {
    let first = unary
    let ignored = first(41)
    let second = nullary
    return second()
}
