module conformance.p43_callable_hole_c32_conditional_false

record Pair {
    left: function() -> Int64 effects { pure },
    right: Text
}

function answer() -> Int64 effects { pure } { return 42 }

function conditional(choose: Bool) -> Int64 effects { pure } {
    let exact = answer
    let pair = Pair { left = exact, right = "ignored" }
    if choose { let moved = pair.left } else { let marker = false }
    return 42
}

@entry
public function launch() -> Int64 effects { pure } { return conditional(false) }
