module conformance.p44e_step_cleanup

record Pair {
    left: function() -> Int64 effects { pure },
    right: Text
}

function answer() -> Int64 effects { pure } { return 42 }

@entry
public function launch() -> Int64 effects { pure } {
    let exact = answer
    var pair = Pair { left = exact, right = "abc" }
    let moved = pair.left
    pair.left = moved
    return 42
}
