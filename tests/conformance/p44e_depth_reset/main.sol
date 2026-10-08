module conformance.p44e_depth_reset

function descend(value: Int64) -> Int64 effects { pure } {
    if value == 0 { return 42 } else { return descend(value - 1) }
}

@entry
public function first() -> Int64 effects { pure } {
    let choice = "fail"
    if choice == "fail" { return descend(63) } else { return 43 }
}
public function second() -> Int64 effects { pure } { return descend(0) }
