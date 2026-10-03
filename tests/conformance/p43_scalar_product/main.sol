module conformance.p43_scalar_product

record Pair { left: Int64, right: Bool }
record Aligned { flag: Bool, value: Int64 }

@entry
public function first() -> Int64 effects { pure } {
    let pair = Pair { left = 20, right = true }
    let aligned = Aligned { flag = true, value = 20 }
    let tuple = (false, 2,)
    return if pair.right && aligned.flag && !tuple.0 {
        pair.left + aligned.value + tuple.1
    } else { 0 }
}

function second() -> Int64 effects { pure } {
    let tuple = (false, 7,)
    return if !tuple.0 { tuple.1 } else { 0 }
}
