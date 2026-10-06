module conformance.p44c_refined_enum_reject

type Positive = refined Int64 where self > 0
enum Envelope { value(item: Positive), empty }

function wrapped(value: Int64) -> Envelope effects { pure } {
    return Envelope.value(Positive(value))
}

@entry
public function launch() -> Int64 effects { pure } {
    let ignored = wrapped(1)
    return 42
}
