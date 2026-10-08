module conformance.p44e_depth_self_63

function descend(value: Int64) -> Int64 effects { pure } {
    if value == 0 { return 42 } else { return descend(value - 1) }
}

@entry
public function launch() -> Int64 effects { pure } { return descend(61) }
