module invalid_ownership

capability Clock {
    function read() -> Int64 effects { pure }
}

function invalid(clock: capability Clock) -> Int64 effects { pure } {
    let alias = clock
    return clock.read()
}
