module conformance.p43_scalar_product_text_reject

record TextBox { value: Text }

@entry
public function first() -> Int64 effects { pure } {
    let boxed = TextBox { value = "text" }
    return 0
}

function second() -> Int64 effects { pure } { return 0 }
