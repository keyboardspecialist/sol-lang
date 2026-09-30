module conformance.p42_scalar_internal

function scalar(value: Int64) -> Int64 effects { pure } {
    let copied = value
    return if copied > 0 { copied } else { 7 }
}

function compound() -> Int64 effects { pure } {
    var value = 1
    value += 2
    return value
}
