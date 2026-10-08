module conformance.p44e_depth_mutual

function left(value: Int64) -> Int64 effects { pure } {
    if value == 0 { return 42 } else { return right(value - 1) }
}
function right(value: Int64) -> Int64 effects { pure } {
    if value == 0 { return 42 } else { return left(value - 1) }
}

@entry
public function launch() -> Int64 effects { pure } { return left(62) }
