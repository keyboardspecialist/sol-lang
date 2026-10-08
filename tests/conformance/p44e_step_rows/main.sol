module conformance.p44e_step_rows

@entry
public function launch() -> Int64 effects { pure }
requires { if true { 1 + 1 == 2 } else { false } }
ensures { result == 42 } {
    let value = 40
    return value + 2
}
