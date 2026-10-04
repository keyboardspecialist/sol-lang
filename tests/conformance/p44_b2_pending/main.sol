module conformance.p44_b2_pending

private function inner() -> Int64 effects { panic } {
    panic "pending inner"
}

@entry
public function launch() -> Int64 effects { panic } {
    let held = "caller-owned"
    let result = inner()
    if held == "caller-owned" {
        result
    } else {
        0
    }
}
