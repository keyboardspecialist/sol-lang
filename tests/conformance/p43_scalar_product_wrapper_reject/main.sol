module conformance.p43_scalar_product_wrapper_reject

type Token = distinct Int64

@entry
public function first() -> Int64 effects { pure } { return 0 }

function second() -> Int64 effects { pure } {
    let token = Token(1)
    return 0
}
