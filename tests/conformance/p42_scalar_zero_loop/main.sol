module conformance.p42_scalar_zero_loop

@entry
public function launch() -> Int64 effects { pure } {
    var score = 31
    var running = false
    while running decreases { score } {
        running = false
    }
    return score
}
