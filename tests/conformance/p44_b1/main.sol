module conformance.p44_b1

function unit_route(value: Bool) -> () effects { pure } {
    if value {
        return ()
    } else {
        return ()
    }
}

@entry
public function launch() -> Int64 effects { pure } {
    var score = 0
    var current = 0
    var choose = true
    if choose {
        score = 40
    } else {
        score = 1
    }
    choose = false
    if choose {
        score = 2
    } else {
        score += 1
    }
    while current < 4 decreases { 4 - current } {
        current += 1
        if current == 1 {
            continue
        } else {
            ()
        }
        if current == 3 {
            break
        } else {
            ()
        }
        score += current
    }
    unit_route(true)
    unit_route(false)
    return score
}
