public class TestLogicalAnd {
    public static void main(String[] args) {
        int x = 20;
        System.out.println(!(x == 20 && args[0].size() > 5));
        System.out.println(!(((((x == 20)) && ((args[0].size() > 5))))));
    }
}
