module demo.main
edition 2027

use core.Text

@stable("demo.User.v1")
public record User {
    name: Text,
}

enum GreetError {
    invalid(field: Text),
}

capability Clock {
    function now() -> Int64
    effects {
        clock.read<Self>
    }
}

public function greet(
    user: User,
    clock: capability Clock,
) -> Result<Text, GreetError>
effects {
    clock.read<clock>
}
requires {
    true
}
ensures {
    true
} {
    return ok(user.name)
}
