module conformance.p44c_ensures_order

@entry
public function launch() -> Int64 effects { pure } ensures {
    false
    result / 0 > 0
} {
    return 42
}
