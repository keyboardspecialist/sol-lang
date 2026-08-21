module invalid_borrow

capability Token {
    function read() -> Int64 effects { pure }
}

function conflict(left: inout capability Token, right: borrow capability Token) -> Int64 {
    return 0
}

function invalid(value: capability Token) -> Int64 {
    var local = value
    return conflict(local, local)
}
