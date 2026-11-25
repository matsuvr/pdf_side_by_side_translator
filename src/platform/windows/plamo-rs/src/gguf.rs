use crate::{Result, err};
use std::{collections::HashMap, fs::File, path::Path};

#[derive(Debug)]
pub struct Tensor {
    pub dims: Vec<usize>,
    pub ty: u32,
    pub offset: usize,
    pub len: usize,
}
#[derive(Debug)]
pub enum Value {
    U(u64),
    F(f32),
    B(bool),
    S(String),
    Strings(Vec<String>),
    Floats(Vec<f32>),
    Ints(Vec<u32>),
    Other,
}
pub struct Gguf {
    map: Vec<u8>,
    pub meta: HashMap<String, Value>,
    pub tensors: HashMap<String, Tensor>,
}
struct Reader<'a> {
    b: &'a [u8],
    p: usize,
}
impl<'a> Reader<'a> {
    fn bytes(&mut self, n: usize) -> Result<&'a [u8]> {
        let end = self
            .p
            .checked_add(n)
            .ok_or_else(|| err("offset overflow"))?;
        let v = self
            .b
            .get(self.p..end)
            .ok_or_else(|| err("truncated GGUF"))?;
        self.p = end;
        Ok(v)
    }
    fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.bytes(4)?.try_into()?))
    }
    fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_le_bytes(self.bytes(8)?.try_into()?))
    }
    fn size(&mut self) -> Result<usize> {
        Ok(usize::try_from(self.u64()?)?)
    }
    fn str(&mut self) -> Result<String> {
        let n = self.size()?;
        if n > 16 * 1024 * 1024 {
            return Err(err("string too large"));
        }
        Ok(std::str::from_utf8(self.bytes(n)?)?.to_owned())
    }
    fn value(&mut self, t: u32) -> Result<Value> {
        Ok(match t {
            0 | 1 => Value::U(self.bytes(1)?[0] as u64),
            2 | 3 => Value::U(u16::from_le_bytes(self.bytes(2)?.try_into()?) as u64),
            4 | 5 => Value::U(self.u32()? as u64),
            6 => Value::F(f32::from_bits(self.u32()?)),
            7 => Value::B(self.bytes(1)?[0] != 0),
            8 => Value::S(self.str()?),
            9 => {
                let ty = self.u32()?;
                let n = self.size()?;
                if n > 1_000_000 {
                    return Err(err("oversized array"));
                }
                match ty {
                    8 => Value::Strings((0..n).map(|_| self.str()).collect::<Result<_>>()?),
                    6 => Value::Floats(
                        (0..n)
                            .map(|_| Ok(f32::from_bits(self.u32()?)))
                            .collect::<Result<_>>()?,
                    ),
                    4 | 5 => Value::Ints((0..n).map(|_| self.u32()).collect::<Result<_>>()?),
                    0 | 1 | 7 => {
                        self.bytes(n)?;
                        Value::Other
                    }
                    2 | 3 => {
                        self.bytes(n.checked_mul(2).ok_or_else(|| err("array overflow"))?)?;
                        Value::Other
                    }
                    10..=12 => {
                        self.bytes(n.checked_mul(8).ok_or_else(|| err("array overflow"))?)?;
                        Value::Other
                    }
                    _ => return Err(err("unsupported array type")),
                }
            }
            10 | 11 => Value::U(self.u64()?),
            12 => {
                self.bytes(8)?;
                Value::Other
            }
            _ => return Err(err("unsupported metadata type")),
        })
    }
}
impl Gguf {
    pub fn open(path: &Path) -> Result<Self> {
        let f = File::open(path)?;
        let size = usize::try_from(f.metadata()?.len())?;
        if size > 1024 * 1024 * 1024 { return Err(err("model too large")); }
        use std::io::Read;
        let mut map = Vec::new();
        map.try_reserve_exact(size)?;
        f.take(size as u64 + 1).read_to_end(&mut map)?;
        if map.len() != size { return Err(err("model size changed")); }
        Self::from_bytes(map)
    }
    pub(crate) fn from_bytes(map: Vec<u8>) -> Result<Self> {
        let mut r = Reader { b: &map, p: 0 };
        if r.bytes(4)? != b"GGUF" || r.u32()? != 3 {
            return Err(err("expected GGUF v3"));
        }
        let nt = r.size()?;
        let nm = r.size()?;
        if nt > 512 || nm > 1024 {
            return Err(err("unexpected GGUF counts"));
        }
        let mut meta = HashMap::new();
        for _ in 0..nm {
            let k = r.str()?;
            let t = r.u32()?;
            let v = r.value(t)?;
            if meta.insert(k, v).is_some() {
                return Err(err("duplicate metadata"));
            }
        }
        let mut tensors = HashMap::new();
        for _ in 0..nt {
            let name = r.str()?;
            let nd = r.u32()?;
            if !(1..=2).contains(&nd) {
                return Err(err("unexpected tensor rank"));
            }
            let dims = (0..nd).map(|_| r.size()).collect::<Result<Vec<_>>>()?;
            let ty = r.u32()?;
            let offset = r.size()?;
            let (b, sz): (usize, usize) = match ty {
                0 => (1, 4),
                12 => (256, 144),
                14 => (256, 210),
                _ => return Err(err(format!("unsupported tensor type {ty}"))),
            };
            if dims[0] == 0 || dims[0] % b != 0 || dims.iter().any(|&d| d == 0) {
                return Err(err("bad tensor dimensions"));
            }
            let len = dims
                .iter()
                .try_fold(sz, |acc, d| acc.checked_mul(*d))
                .ok_or_else(|| err("tensor size overflow"))?
                / b;
            if tensors
                .insert(
                    name,
                    Tensor {
                        dims,
                        ty,
                        offset,
                        len,
                    },
                )
                .is_some()
            {
                return Err(err("duplicate tensor"));
            }
        }
        let align = match meta.get("general.alignment") {
            Some(Value::U(x)) => usize::try_from(*x)?,
            None => 32,
            _ => return Err(err("bad alignment")),
        };
        if !align.is_power_of_two() || align > 4096 {
            return Err(err("bad alignment"));
        }
        let data = (r.p + align - 1) & !(align - 1);
        for t in tensors.values_mut() {
            if t.offset % align != 0 {
                return Err(err("unaligned tensor"));
            }
            t.offset = t
                .offset
                .checked_add(data)
                .ok_or_else(|| err("tensor overflow"))?;
            if t.offset
                .checked_add(t.len)
                .is_none_or(|end| end > map.len())
            {
                return Err(err("truncated tensor"));
            }
        }
        let mut spans = tensors
            .values()
            .map(|t| (t.offset, t.offset + t.len))
            .collect::<Vec<_>>();
        spans.sort_unstable();
        if spans.windows(2).any(|x| x[0].1 > x[1].0) {
            return Err(err("overlapping tensors"));
        }
        Ok(Self { map, meta, tensors })
    }
    pub fn tensor(&self, name: &str, dims: &[usize], types: &[u32]) -> Result<&Tensor> {
        let t = self
            .tensors
            .get(name)
            .ok_or_else(|| err(format!("missing {name}")))?;
        if t.dims != dims || !types.contains(&t.ty) {
            return Err(err(format!("bad tensor {name}: {t:?}")));
        }
        Ok(t)
    }
    pub fn data(&self, t: &Tensor) -> &[u8] {
        &self.map[t.offset..t.offset + t.len]
    }
    pub fn u(&self, k: &str) -> Result<u64> {
        match self.meta.get(k) {
            Some(Value::U(n)) => Ok(*n),
            _ => Err(err(format!("missing integer {k}"))),
        }
    }
    pub fn s(&self, k: &str) -> Result<&str> {
        match self.meta.get(k) {
            Some(Value::S(s)) => Ok(s),
            _ => Err(err(format!("missing string {k}"))),
        }
    }
    pub fn float(&self, k: &str) -> Result<f32> {
        match self.meta.get(k) {
            Some(Value::F(n)) => Ok(*n),
            _ => Err(err(format!("missing float {k}"))),
        }
    }
}
