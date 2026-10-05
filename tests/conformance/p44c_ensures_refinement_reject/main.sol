module conformance.p44c_ensures_refinement_reject

type Positive = refined Int64 where self > 0

@entry
public function launch() -> Int64 effects { pure } ensures {
    Positive(result) == Positive(result)
} {
    return 42
}
