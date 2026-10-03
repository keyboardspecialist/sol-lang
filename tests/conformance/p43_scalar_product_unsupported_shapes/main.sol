module conformance.p43_scalar_product_unsupported_shapes

record Leaf { value: Int64 }
record Nested { leaf: Leaf }

@entry
public function first() -> Int64 effects { pure } {
    let leaf = Leaf { value = 1 }
    let nested = Nested { leaf = leaf }
    return 0
}

function second() -> Int64 effects { pure } { return 0 }
