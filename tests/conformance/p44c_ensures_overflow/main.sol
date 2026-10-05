module conformance.p44c_ensures_overflow

@entry
public function launch() -> Int64 effects { pure } ensures {
    result + 9223372036854775807 > 0
} {
    return 1
}
