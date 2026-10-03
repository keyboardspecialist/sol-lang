module conformance.p43_callable_hole_prereq

record Pair {
    left: function() -> Int64 effects { pure },
    right: Text
}

function answer() -> Int64 effects { pure } { return 42 }

@entry
public function launch() -> Int64 effects { pure } {
    let exact = answer
    let pair = Pair { left = exact, right = "ignored" }
    let moved = pair.left
    return moved()
}
