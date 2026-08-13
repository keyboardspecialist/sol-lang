module invalid_effect

function read_clock() -> Int64 effects { panic } {
    return 1
}

function caller() -> Int64 effects { pure } {
    return read_clock()
}
