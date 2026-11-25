# Interaction rules

The device has a knob (turn and press) and a round touch screen. Every screen follows the same rules, taken from the Nest thermostat pattern.

- **Rotate** changes the selected thing or the value being edited (a list's selection, timer minutes, a score). It never moves between screens and never reveals actions.
- **Press** selects or confirms the highlighted thing. It's the primary action.
- **Swipe** left or right moves between panels. On a sub-screen it backs out (games ignore swipes).
- **Tap** acts on the control touched, at once. A record's actions are tappable on the record itself.
- **Hold** (0.7 s) backs out of a sub-screen; a 3 s hold of the knob or a still finger goes Home from anywhere.
- **No one-action screens** (a full screen with a single button) and no hints like "turn for actions".
- **No undo or confirm for non-destructive changes.** Let people change it again. Ask before adding a confirm step, and only for something truly destructive.
- **Knob acceleration wherever a value or list moves far:** slow clicks are fine steps (1 minute, 1 key), fast spins cover ground (snap to multiples of 5, up to 4x). It responds on the click itself; never queue steps to play out later.
- **Every dial takes touch:** tap a point on the ring or drag along it, in a generous band in from the edge; movement across the ring is still a swipe.
- **Every list scrolls by touch** (drag and flick like a phone), including pick-one sub-menus. "Custom" goes first in new-item pickers.
- **The knob's outline shows only while the knob is in use.** After a touch it hides; the first turn brings it back without moving.
- **Common actions get their own touch target** even when the knob can reach them (delete at the end of the text line).
- **Detail screens have Back before Remove.** A screen whose only button is Remove has no way out.
