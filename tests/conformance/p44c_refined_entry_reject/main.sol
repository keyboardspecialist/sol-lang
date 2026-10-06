module conformance.p44c_refined_entry_reject

type Positive = refined Int64 where self > 0

@entry
public function launch() -> Positive effects { pure } {
    return Positive(1)
}
