module conformance.p43_scalar_product_copy_reject

record Pair { left: Int64, right: Bool }

@entry
public function first() -> Int64 effects { pure } {
    let pair = Pair { left = 1, right = true }
    let copied = pair
    return copied.left
}

function second() -> Int64 effects { pure } { return 0 }
