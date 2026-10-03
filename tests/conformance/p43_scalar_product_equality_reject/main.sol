module conformance.p43_scalar_product_equality_reject

record Pair { left: Int64, right: Bool }

@entry
public function first() -> Int64 effects { pure } {
    let left = Pair { left = 1, right = true }
    let right = Pair { left = 1, right = true }
    return if left == right { 1 } else { 0 }
}

function second() -> Int64 effects { pure } { return 0 }
