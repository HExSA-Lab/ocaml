(* TEST *)

let () =
  let near = Array.make 16 2.0 in
  let far = Array.make ~far:true 16 2.0 in
  for i = 0 to 15 do
    assert (far.(i) = 2.0)
  done;
  assert (Obj.tag (Obj.repr far) = Obj.tag (Obj.repr near));
  Array.fill far 0 16 3.0;
  for i = 0 to 15 do
    assert (far.(i) = 3.0)
  done;
  let ints = Array.make ~far:true 16 7 in
  for i = 0 to 15 do
    assert (ints.(i) = 7)
  done
