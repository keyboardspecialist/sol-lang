module inspection_tuple

function inspect() -> Int64 effects { pure } {
    let value = ("tuple", 42, true,)
    return value.1
}
