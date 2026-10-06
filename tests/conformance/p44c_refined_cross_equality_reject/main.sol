module conformance.p44c_refined_cross_equality_reject

type Left = refined Int64 where self > 0
type Right = refined Int64 where self > 0

@entry
public function launch() -> Int64 effects { pure } {
    let left = Left(1)
    let right = Right(1)
    if left == right { return 42 } else { return 0 }
}
