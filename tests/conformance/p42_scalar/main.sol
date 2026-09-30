module conformance.p42_scalar

@entry
public function launch() -> Int64 effects { pure } {
    let score = if true { 29 } else { 1 }
    return score
}
