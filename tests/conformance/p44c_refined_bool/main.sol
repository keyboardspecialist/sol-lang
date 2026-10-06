module conformance.p44c_refined_bool

type Enabled = refined Bool where self

function enabled(value: Bool) -> Enabled effects { pure } {
    return Enabled(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let left = enabled(true)
    let right = enabled(true)
    if left == right { return 1 } else { return 0 }
}
