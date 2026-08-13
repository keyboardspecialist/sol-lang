module invalid_authority

function read_clock() -> Int64 effects { clock.read } {
    return 1
}
