module conformance.p43_callable_hole_exact

record Pair {
    left: function() -> Int64 effects { pure },
    right: Text
}

function answer() -> Int64 effects { pure } { return 42 }

function conditional(choose: Bool) -> Int64 effects { pure } {
    let exact = answer
    let pair = Pair { left = exact, right = "ignored" }
    if choose {
        let moved = pair.left
    } else {
        let marker = false
    }
    return 42
}

@entry
public function conditional_true() -> Int64 effects { pure } {
    return conditional(true)
}

public function conditional_false() -> Int64 effects { pure } {
    return conditional(false)
}

public function repair() -> Int64 effects { pure } {
    let exact = answer
    var pair = Pair { left = exact, right = "ignored" }
    let moved = pair.left
    pair.left = moved
    return 42
}

public function reopen() -> Int64 effects { pure } {
    let exact = answer
    var pair = Pair { left = exact, right = "ignored" }
    let moved = pair.left
    pair.left = moved
    let reopened = pair.left
    return 42
}

public function whole_root() -> Int64 effects { pure } {
    let exact = answer
    let pair = Pair { left = exact, right = "ignored" }
    let destination = pair
    return 42
}
