module conformance.p43_scalar_product_sum_pattern_reject

enum Payload { item(value: Text) }

@entry
public function first() -> Int64 effects { pure } { return 0 }

function second(value: Payload, unused: Bool) -> Text effects { pure } {
    return match value {
        item(selected) => selected
    }
}
